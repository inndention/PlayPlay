import React, { useState, useRef } from 'react';
import { loadSettings, saveSettings } from '../lib/storage';
import { runFacebookAction } from '../lib/facebook';
import { generateStoryContent, generateStoryVariations } from '../lib/openai';
import { Sparkles, Send, RefreshCw, Loader, AlertCircle, CheckCircle, Image, Upload, X } from 'lucide-react';
import styles from './FacebookStoryPoster.module.css';

interface FacebookStoryPosterProps {
    currentProgram?: string;
    onClose?: () => void;
}

export function FacebookStoryPoster({ currentProgram, onClose }: FacebookStoryPosterProps) {
    const settings = loadSettings();
    const [prompt, setPrompt] = useState('');
    const [generatedContent, setGeneratedContent] = useState<string>('');
    const [variations, setVariations] = useState<string[]>([]);
    const [imageUrl, setImageUrl] = useState('');
    const [uploadedImage, setUploadedImage] = useState<{ file: File; preview: string } | null>(null);
    const [isGenerating, setIsGenerating] = useState(false);
    const [isPosting, setIsPosting] = useState(false);
    const [status, setStatus] = useState<{ type: 'success' | 'error'; message: string } | null>(null);
    const fileInputRef = useRef<HTMLInputElement>(null);

    const hasOpenAI = !!(settings.openaiApiKey && settings.openaiApiKey.trim());
    const hasFacebook = !!(settings.facebookGraphPageId && settings.facebookGraphAccessToken) || 
                        !!(settings.facebookWebhookUrl && settings.facebookWebhookEnabled);

    const handleGenerate = async () => {
        if (!hasOpenAI) {
            setStatus({ type: 'error', message: 'OpenAI API key not configured. Set it in Settings.' });
            return;
        }

        if (!prompt.trim()) {
            setStatus({ type: 'error', message: 'Please enter a prompt' });
            return;
        }

        setIsGenerating(true);
        setStatus(null);

        try {
            const timeOfDay = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
            const result = await generateStoryContent(prompt, settings.openaiApiKey!, {
                currentProgram,
                timeOfDay,
            });

            if (result.success && result.content) {
                setGeneratedContent(result.content);
                setStatus({ type: 'success', message: 'Story content generated!' });
            } else {
                setStatus({ type: 'error', message: result.error || 'Failed to generate content' });
            }
        } catch (error) {
            setStatus({ type: 'error', message: error instanceof Error ? error.message : 'Unknown error' });
        } finally {
            setIsGenerating(false);
        }
    };

    const handleGenerateVariations = async () => {
        if (!hasOpenAI) {
            setStatus({ type: 'error', message: 'OpenAI API key not configured' });
            return;
        }

        if (!prompt.trim()) {
            setStatus({ type: 'error', message: 'Please enter a prompt' });
            return;
        }

        setIsGenerating(true);
        setStatus(null);

        try {
            const timeOfDay = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
            const result = await generateStoryVariations(prompt, settings.openaiApiKey!, 3, {
                currentProgram,
                timeOfDay,
            });

            if (result.success && result.variations) {
                setVariations(result.variations);
                if (result.variations.length > 0) {
                    setGeneratedContent(result.variations[0]);
                }
                setStatus({ type: 'success', message: `Generated ${result.variations.length} variations!` });
            } else {
                setStatus({ type: 'error', message: result.error || 'Failed to generate variations' });
            }
        } catch (error) {
            setStatus({ type: 'error', message: error instanceof Error ? error.message : 'Unknown error' });
        } finally {
            setIsGenerating(false);
        }
    };

    const handlePost = async () => {
        if (!hasFacebook) {
            setStatus({ type: 'error', message: 'Facebook not configured. Set it in Settings.' });
            return;
        }

        if (!generatedContent.trim()) {
            setStatus({ type: 'error', message: 'No content to post. Generate content first.' });
            return;
        }

        setIsPosting(true);
        setStatus(null);

        try {
            await runFacebookAction({
                type: 'story',
                message: generatedContent,
                imageUrl: imageUrl.trim() || undefined,
            });

            setStatus({ type: 'success', message: 'Story posted successfully!' });
            
            // Clear form after successful post
            setTimeout(() => {
                setGeneratedContent('');
                setVariations([]);
                setPrompt('');
                setImageUrl('');
                setStatus(null);
            }, 2000);
        } catch (error) {
            setStatus({ type: 'error', message: error instanceof Error ? error.message : 'Failed to post story' });
        } finally {
            setIsPosting(false);
        }
    };

    const selectVariation = (content: string) => {
        setGeneratedContent(content);
    };

    const handleImageUpload = (e: React.ChangeEvent<HTMLInputElement>) => {
        const file = e.target.files?.[0];
        if (!file) return;
        
        // Validate file type
        if (!file.type.startsWith('image/')) {
            setStatus({ type: 'error', message: 'Please select an image file' });
            return;
        }
        
        // Create preview
        const preview = URL.createObjectURL(file);
        setUploadedImage({ file, preview });
        setImageUrl(''); // Clear URL input when file is uploaded
    };

    const handleRemoveImage = () => {
        if (uploadedImage?.preview) {
            URL.revokeObjectURL(uploadedImage.preview);
        }
        setUploadedImage(null);
        if (fileInputRef.current) {
            fileInputRef.current.value = '';
        }
    };

    const triggerImageUpload = () => {
        fileInputRef.current?.click();
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Sparkles size={18} />
                <h3>AI-Assisted Facebook Story</h3>
                {onClose && (
                    <button className={styles.closeBtn} onClick={onClose}>×</button>
                )}
            </div>

            {!hasOpenAI && (
                <div className={styles.warning}>
                    <AlertCircle size={16} />
                    <span>OpenAI API key not configured. Set it in Settings → Facebook Automation.</span>
                </div>
            )}

            {!hasFacebook && (
                <div className={styles.warning}>
                    <AlertCircle size={16} />
                    <span>Facebook not configured. Set it in Settings → Facebook Automation.</span>
                </div>
            )}

            <div className={styles.form}>
                <label className={styles.label}>
                    Prompt / Context
                    <textarea
                        className={styles.textarea}
                        value={prompt}
                        onChange={e => setPrompt(e.target.value)}
                        placeholder="e.g., 'Promote the upcoming news segment about local weather' or 'Create a story about breaking news'"
                        rows={3}
                    />
                </label>

                {currentProgram && (
                    <div className={styles.context}>
                        <span>Current Program: <strong>{currentProgram}</strong></span>
                    </div>
                )}

                <div className={styles.buttonRow}>
                    <button
                        className={styles.generateBtn}
                        onClick={handleGenerate}
                        disabled={isGenerating || !hasOpenAI || !prompt.trim()}
                    >
                        {isGenerating ? (
                            <>
                                <Loader size={14} className={styles.spinner} />
                                Generating...
                            </>
                        ) : (
                            <>
                                <Sparkles size={14} />
                                Generate Story
                            </>
                        )}
                    </button>

                    <button
                        className={styles.variationsBtn}
                        onClick={handleGenerateVariations}
                        disabled={isGenerating || !hasOpenAI || !prompt.trim()}
                    >
                        {isGenerating ? (
                            <>
                                <Loader size={14} className={styles.spinner} />
                                Generating...
                            </>
                        ) : (
                            <>
                                <RefreshCw size={14} />
                                Generate 3 Variations
                            </>
                        )}
                    </button>
                </div>

                {variations.length > 0 && (
                    <div className={styles.variations}>
                        <label className={styles.label}>Choose a variation:</label>
                        {variations.map((variation, idx) => (
                            <button
                                key={idx}
                                className={`${styles.variationBtn} ${generatedContent === variation ? styles.selected : ''}`}
                                onClick={() => selectVariation(variation)}
                            >
                                {variation}
                            </button>
                        ))}
                    </div>
                )}

                {generatedContent && (
                    <>
                        <label className={styles.label}>
                            Generated Story Content
                            <textarea
                                className={styles.textarea}
                                value={generatedContent}
                                onChange={e => setGeneratedContent(e.target.value)}
                                rows={4}
                                maxLength={220}
                            />
                            <div className={styles.charCount}>
                                {generatedContent.length} / 220 characters
                            </div>
                        </label>

                        <div className={styles.imageSection}>
                            <label className={styles.label}>Image (optional)</label>
                            
                            <div className={styles.imageOptions}>
                                <button
                                    type="button"
                                    className={styles.uploadBtn}
                                    onClick={triggerImageUpload}
                                >
                                    <Upload size={16} />
                                    Upload Image
                                </button>
                                <span className={styles.orDivider}>or</span>
                                <input
                                    type="url"
                                    className={styles.urlInput}
                                    value={imageUrl}
                                    onChange={e => {
                                        setImageUrl(e.target.value);
                                        if (uploadedImage) handleRemoveImage();
                                    }}
                                    placeholder="Paste image URL..."
                                    disabled={!!uploadedImage}
                                />
                            </div>
                            
                            <input
                                ref={fileInputRef}
                                type="file"
                                accept="image/*"
                                onChange={handleImageUpload}
                                style={{ display: 'none' }}
                            />
                            
                            {(uploadedImage || imageUrl) && (
                                <div className={styles.imagePreviewContainer}>
                                    <img 
                                        src={uploadedImage?.preview || imageUrl}
                                        alt="Preview"
                                        className={styles.imagePreview}
                                        onError={(e) => {
                                            (e.target as HTMLImageElement).style.display = 'none';
                                        }}
                                    />
                                    <button
                                        type="button"
                                        className={styles.removeImageBtn}
                                        onClick={() => {
                                            if (uploadedImage) handleRemoveImage();
                                            else setImageUrl('');
                                        }}
                                    >
                                        <X size={14} />
                                    </button>
                                </div>
                            )}
                        </div>

                        <button
                            className={styles.postBtn}
                            onClick={handlePost}
                            disabled={isPosting || !hasFacebook || !generatedContent.trim()}
                        >
                            {isPosting ? (
                                <>
                                    <Loader size={14} className={styles.spinner} />
                                    Posting...
                                </>
                            ) : (
                                <>
                                    <Send size={14} />
                                    Post to Facebook Story
                                </>
                            )}
                        </button>
                    </>
                )}
            </div>

            {status && (
                <div className={`${styles.status} ${styles[status.type]}`}>
                    {status.type === 'success' ? <CheckCircle size={16} /> : <AlertCircle size={16} />}
                    <span>{status.message}</span>
                </div>
            )}
        </div>
    );
}






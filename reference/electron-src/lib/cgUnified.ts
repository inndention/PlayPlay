import type {
    CGAnchorPreset,
    CGElement,
    CGPosition,
    CGTemplate,
    UnifiedCGCoreFields,
    UnifiedCGFieldDefinition,
    UnifiedCGLayerState,
    UnifiedCGSourceTemplate,
} from './types';
import { CG_TEMPLATES } from './cgTemplates';

const DEFAULT_STYLE_1 = '#0f172a';
const DEFAULT_STYLE_2 = '#2563eb';
const DEFAULT_STYLE_3 = '#f8fafc';

function clamp(value: number, min: number, max: number): number {
    return Math.min(max, Math.max(min, value));
}

function normalizeDefaultValue(value: unknown): string {
    if (value === undefined || value === null) {
        return '';
    }
    return String(value);
}

function findField(
    fields: UnifiedCGFieldDefinition[],
    used: Set<string>,
    patterns: RegExp[],
): string | undefined {
    const match = fields.find((field) => !used.has(field.key) && patterns.some((pattern) => pattern.test(field.key)));
    if (!match) {
        return undefined;
    }
    used.add(match.key);
    return match.key;
}

function guessTemplateFieldMap(fields: UnifiedCGFieldDefinition[]) {
    const used = new Set<string>();
    return {
        text1: findField(fields, used, [/name/i, /headline/i, /^title$/i, /^text$/i, /message/i, /team1/i, /channel/i]),
        text2: findField(fields, used, [/subtitle/i, /role/i, /position/i, /team2/i, /label/i, /company/i]),
        text3: findField(fields, used, [/stat/i, /period/i, /time/i, /score/i, /credits/i, /items/i, /temp/i, /scene/i]),
        style1: findField(fields, used, [/color1/i, /primary/i, /^color$/i, /background/i]),
        style2: findField(fields, used, [/color2/i, /secondary/i, /accent/i]),
        style3: findField(fields, used, [/color3/i, /textcolor/i, /text_color/i, /foreground/i]),
    };
}

export function createDefaultCoreFields(overrides: Partial<UnifiedCGCoreFields> = {}): UnifiedCGCoreFields {
    return {
        text1: '',
        text2: '',
        text3: '',
        style1: DEFAULT_STYLE_1,
        style2: DEFAULT_STYLE_2,
        style3: DEFAULT_STYLE_3,
        animationIn: 'fade',
        animationLoop: 'none',
        animationOut: 'fade',
        anchorPreset: 'lower-third',
        margin: 4,
        width: 42,
        height: 14,
        ...overrides,
    };
}

export function normalizeTemplateToSource(template: CGTemplate): UnifiedCGSourceTemplate {
    const fields: UnifiedCGFieldDefinition[] = (template.parameters || []).map((parameter) => {
        const parameterType = String(parameter.type || 'text');
        return {
            key: parameter.name,
            label: parameter.label || parameter.name,
            type: parameterType === 'textarea'
            ? 'textarea'
            : parameterType === 'color'
                ? 'color'
                : parameterType === 'number'
                    ? 'number'
                    : parameterType === 'select'
                        ? 'select'
                        : parameterType === 'url'
                            ? 'url'
                            : parameterType === 'datetime'
                                ? 'datetime'
                                : parameterType === 'checkbox'
                                    ? 'checkbox'
                                    : 'text',
            defaultValue: normalizeDefaultValue(parameter.defaultValue),
            options: parameter.options || [],
        };
    });

    const fieldMap = guessTemplateFieldMap(fields);
    const extras = fields.filter((field) => !Object.values(fieldMap).includes(field.key));
    const defaults = Object.fromEntries(fields.map((field) => [field.key, field.defaultValue || '']));

    return {
        templateId: template.id,
        templateName: template.name,
        category: template.category,
        defaults,
        fields: extras,
        fieldMap,
    };
}

export function createTemplateDraftLayer(
    template: UnifiedCGSourceTemplate,
    overrides: Partial<UnifiedCGLayerState> = {},
): UnifiedCGLayerState {
    const core = createDefaultCoreFields({
        text1: template.fieldMap?.text1 ? template.defaults[template.fieldMap.text1] || '' : '',
        text2: template.fieldMap?.text2 ? template.defaults[template.fieldMap.text2] || '' : '',
        text3: template.fieldMap?.text3 ? template.defaults[template.fieldMap.text3] || '' : '',
        style1: template.fieldMap?.style1 ? template.defaults[template.fieldMap.style1] || DEFAULT_STYLE_1 : DEFAULT_STYLE_1,
        style2: template.fieldMap?.style2 ? template.defaults[template.fieldMap.style2] || DEFAULT_STYLE_2 : DEFAULT_STYLE_2,
        style3: template.fieldMap?.style3 ? template.defaults[template.fieldMap.style3] || DEFAULT_STYLE_3 : DEFAULT_STYLE_3,
    });

    const advancedValues = Object.fromEntries(
        template.fields.map((field) => [field.key, template.defaults[field.key] || field.defaultValue || '']),
    );

    return {
        id: overrides.id || `cg_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`,
        type: 'template',
        templateId: template.templateId,
        isVisible: true,
        transition: overrides.transition || 'cut',
        transitionDuration: overrides.transitionDuration || 300,
        layer: overrides.layer || 1,
        displayName: template.templateName,
        templateCategory: template.category,
        channel: overrides.channel,
        core,
        advancedValues,
        fieldDefinitions: template.fields,
        ...overrides,
    };
}

export function createElementDraftLayer(overrides: Partial<UnifiedCGLayerState> = {}): UnifiedCGLayerState {
    const core = createDefaultCoreFields({
        text1: 'Primary Text',
        text2: 'Secondary Text',
        style1: '#0f172a',
        style2: '#2563eb',
        style3: '#ffffff',
    });

    return {
        id: overrides.id || `cg_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`,
        type: 'element',
        isVisible: true,
        transition: overrides.transition || 'cut',
        transitionDuration: overrides.transitionDuration || 300,
        layer: overrides.layer || 1,
        displayName: overrides.displayName || 'Element CG',
        channel: overrides.channel,
        core,
        advancedValues: {},
        fieldDefinitions: [],
        ...overrides,
    };
}

export function buildTemplateQuery(
    template: UnifiedCGSourceTemplate,
    core: UnifiedCGCoreFields,
    advancedValues: Record<string, string> = {},
): URLSearchParams {
    const params = new URLSearchParams();
    const merged = { ...template.defaults, ...advancedValues };

    Object.entries(merged).forEach(([key, value]) => {
        if (value !== undefined && value !== null && String(value) !== '') {
            params.set(key, String(value));
        }
    });

    if (template.fieldMap?.text1) params.set(template.fieldMap.text1, core.text1);
    if (template.fieldMap?.text2) params.set(template.fieldMap.text2, core.text2);
    if (template.fieldMap?.text3) params.set(template.fieldMap.text3, core.text3);
    if (template.fieldMap?.style1) params.set(template.fieldMap.style1, core.style1);
    if (template.fieldMap?.style2) params.set(template.fieldMap.style2, core.style2);
    if (template.fieldMap?.style3) params.set(template.fieldMap.style3, core.style3);

    params.set('style1', core.style1);
    params.set('style2', core.style2);
    params.set('style3', core.style3);
    params.set('animIn', core.animationIn);
    params.set('loop', core.animationLoop === 'none' ? '' : '1');
    params.set('animOut', core.animationOut);
    params.set('anchorPreset', core.anchorPreset);
    params.set('margin', String(core.margin));
    params.set('width', String(core.width));
    params.set('height', String(core.height));

    return params;
}

export function buildTemplateUrl(baseUrl: string, templateId: string, params: URLSearchParams): string {
    const query = params.toString();
    
    // Check if it's one of our local templates
    if (CG_TEMPLATES[templateId]) {
        return `${baseUrl.replace(/\/$/, '')}/cg/${templateId}.html${query ? `?${query}` : ''}`;
    }
    
    return `${baseUrl.replace(/\/$/, '')}/cg-template/${templateId}${query ? `?${query}` : ''}`;
}

function resolveAnchorPosition(core: UnifiedCGCoreFields): CGPosition {
    const width = clamp(core.width, 8, 100);
    const height = clamp(core.height, 4, 100);
    const margin = clamp(core.margin, 0, 30);

    const base: CGPosition = {
        x: margin,
        y: margin,
        width,
        height,
        anchor: 'top-left',
    };

    const centerX = clamp((100 - width) / 2, 0, 100);
    const rightX = clamp(100 - width - margin, 0, 100);
    const bottomY = clamp(100 - height - margin, 0, 100);

    switch (core.anchorPreset) {
        case 'top-center':
            return { ...base, x: centerX, anchor: 'top-center' };
        case 'top-right':
            return { ...base, x: rightX, anchor: 'top-right' };
        case 'bottom-left':
            return { ...base, y: bottomY, anchor: 'bottom-left' };
        case 'bottom-center':
            return { ...base, x: centerX, y: bottomY, anchor: 'bottom-center' };
        case 'bottom-right':
            return { ...base, x: rightX, y: bottomY, anchor: 'bottom-right' };
        case 'lower-third':
            return {
                x: margin,
                y: clamp(100 - Math.max(height, 12) - margin, 0, 100),
                width,
                height: Math.max(height, 12),
                anchor: 'bottom-left',
            };
        case 'top-ticker':
            return { x: 0, y: margin, width: 100, height: Math.max(height, 8), anchor: 'top-left' };
        case 'bottom-ticker':
            return { x: 0, y: bottomY, width: 100, height: Math.max(height, 8), anchor: 'bottom-left' };
        case 'bug-top-left':
            return { x: margin, y: margin, width: Math.min(width, 20), height: Math.min(height, 18), anchor: 'top-left' };
        case 'bug-top-right':
            return { x: rightX, y: margin, width: Math.min(width, 20), height: Math.min(height, 18), anchor: 'top-right' };
        case 'bug-bottom-left':
            return { x: margin, y: bottomY, width: Math.min(width, 20), height: Math.min(height, 18), anchor: 'bottom-left' };
        case 'bug-bottom-right':
            return { x: rightX, y: bottomY, width: Math.min(width, 20), height: Math.min(height, 18), anchor: 'bottom-right' };
        case 'top-left':
        default:
            return base;
    }
}

export function createElementFromLayer(layer: UnifiedCGLayerState): CGElement {
    const core = createDefaultCoreFields(layer.core || {});
    const position = resolveAnchorPosition(core);

    return {
        id: layer.sourceElementId || layer.id,
        name: layer.displayName || core.text1 || 'CG Element',
        type: position.width >= 95 ? 'ticker' : 'lower_third',
        title: core.text1,
        subtitle: core.text2 || undefined,
        tickerText: core.text3 || undefined,
        colors: {
            primary: core.style1,
            secondary: core.style2,
            text: core.style3,
            gradient: true,
            gradientDirection: 'horizontal',
        },
        effects: {
            shadow: true,
            shadowColor: '#000000',
            shadowBlur: 18,
            glow: false,
            borderRadius: 10,
            opacity: 0.96,
        },
        position,
        fontSize: position.height >= 12 ? 30 : 20,
        fontFamily: 'Inter',
        animationIn: core.animationIn as CGElement['animationIn'],
        animationOut: core.animationOut as CGElement['animationOut'],
        animationDuration: layer.transitionDuration || 300,
        loop: core.animationLoop === 'none' ? 'none' : 'glow_pulse',
        isVisible: layer.isVisible,
        isPreset: false,
    };
}

export function cloneLayerForChannel(layer: UnifiedCGLayerState, channel: 'preview' | 'program'): UnifiedCGLayerState {
    return {
        ...layer,
        channel,
        element: layer.element ? { ...layer.element } : undefined,
        core: layer.core ? { ...layer.core } : undefined,
        advancedValues: layer.advancedValues ? { ...layer.advancedValues } : undefined,
        fieldDefinitions: layer.fieldDefinitions ? [...layer.fieldDefinitions] : undefined,
    };
}

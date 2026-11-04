export function generateSignal(type: string): MediaStream {
    const canvas = document.createElement('canvas');
    canvas.width = 1920;
    canvas.height = 1080;
    const ctx = canvas.getContext('2d');

    // Audio Context
    const audioCtx = new (window.AudioContext || (window as any).webkitAudioContext)();
    const dest = audioCtx.createMediaStreamDestination();
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();

    osc.connect(gain);
    gain.connect(dest);

    // Default Tone
    osc.type = 'sine';
    osc.frequency.value = 1000; // 1kHz
    gain.gain.value = 0.1; // -20dBFS approx
    osc.start();

    // Draw Pattern
    if (ctx) {
        if (type === 'bars') {
            drawSMPTEBars(ctx, canvas.width, canvas.height);
        } else if (type === 'ramp') {
            drawRamp(ctx, canvas.width, canvas.height);
        } else if (type === 'ball') {
            drawBouncingBall(ctx, canvas.width, canvas.height);
        } else {
            // Black
            ctx.fillStyle = 'black';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
        }
    }

    // Get Video Stream (0 fps for static, 30 for anim)
    const fps = type === 'ball' ? 30 : 0; // 0 means "capture once" effectively for static
    const videoStream = canvas.captureStream(fps);

    // Combine
    const tracks = [
        ...videoStream.getVideoTracks(),
        ...dest.stream.getAudioTracks()
    ];

    return new MediaStream(tracks);
}

function drawSMPTEBars(ctx: CanvasRenderingContext2D, w: number, h: number) {
    const colors = [
        '#c0c0c0', // White (75%)
        '#c0c000', // Yellow
        '#00c0c0', // Cyan
        '#00c000', // Green
        '#c000c0', // Magenta
        '#c00000', // Red
        '#0000c0', // Blue
    ];

    const barWidth = w / 7;

    // Top 2/3
    colors.forEach((c, i) => {
        ctx.fillStyle = c;
        ctx.fillRect(i * barWidth, 0, barWidth, h * 0.67);
    });

    // Bottom patches (Simplified SMPTE)
    const bottomH = h * 0.33;
    const y = h * 0.67;

    // Blue/White/Purple/Black/etc simplified
    const lowerColors = ['#0000c0', '#ffffff', '#320032', '#000000', '#090909', '#000000', '#0000c0'];
    lowerColors.forEach((c, i) => {
        ctx.fillStyle = c;
        ctx.fillRect(i * barWidth, y, barWidth, bottomH);
    });
}

function drawRamp(ctx: CanvasRenderingContext2D, w: number, h: number) {
    const grad = ctx.createLinearGradient(0, 0, w, 0);
    grad.addColorStop(0, 'black');
    grad.addColorStop(1, 'white');
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, w, h);
}

function drawBouncingBall(ctx: CanvasRenderingContext2D, w: number, h: number) {
    // For proper animation, we'd need a loop. `captureStream` handles the loop if we keep drawing.
    // However, `canvas.captureStream` is a live stream. We need to update the canvas.
    // This simple function just draws one frame.
    // To support animation, we would need to return an interval cleaner or handle it externally.
    // For simplicity in this v1, let's keep it static static or handle animation within the `generateSignal` closure if we truly want it.

    // Let's implement a simple loop for 'ball'
    let x = w / 2;
    let y = h / 2;
    let dx = 5;
    let dy = 5;
    const r = 50;

    function animate() {
        ctx.fillStyle = '#111';
        ctx.fillRect(0, 0, w, h);

        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI * 2);
        ctx.fillStyle = 'red';
        ctx.fill();
        ctx.closePath();

        if (x + r > w || x - r < 0) dx = -dx;
        if (y + r > h || y - r < 0) dy = -dy;
        x += dx;
        y += dy;

        requestAnimationFrame(animate);
    }
    animate();
}

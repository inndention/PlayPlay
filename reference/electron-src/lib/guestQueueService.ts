/**
 * Guest Queue Service
 * Manages guest queuing with QR code-based join
 */

export interface QueuedGuest {
    id: string;
    name: string;
    sessionId: string;
    qrCodeUrl: string;
    joinUrl: string;
    status: 'waiting' | 'joined' | 'on-air' | 'completed';
    queuedAt: Date;
    joinedAt?: Date;
    onAirAt?: Date;
    position: number;
}

export class GuestQueueService {
    private queue: QueuedGuest[] = [];
    private baseUrl: string;

    constructor(baseUrl: string = window.location.origin) {
        this.baseUrl = baseUrl;
    }

    /**
     * Add guest to queue
     */
    addGuest(name: string, sessionId?: string): QueuedGuest {
        const guestId = `guest_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        const session = sessionId || `session_${Date.now()}`;
        const joinUrl = `${this.baseUrl}/guest?sessionId=${encodeURIComponent(session)}&name=${encodeURIComponent(name)}`;
        
        const guest: QueuedGuest = {
            id: guestId,
            name,
            sessionId: session,
            qrCodeUrl: this.generateQRCode(joinUrl),
            joinUrl,
            status: 'waiting',
            queuedAt: new Date(),
            position: this.queue.length + 1,
        };

        this.queue.push(guest);
        return guest;
    }

    /**
     * Generate QR code URL (using a QR code API)
     */
    private generateQRCode(url: string): string {
        // Using a free QR code API
        return `https://api.qrserver.com/v1/create-qr-code/?size=300x300&data=${encodeURIComponent(url)}`;
    }

    /**
     * Get guest by ID
     */
    getGuest(guestId: string): QueuedGuest | undefined {
        return this.queue.find(g => g.id === guestId);
    }

    /**
     * Get all queued guests
     */
    getQueue(): QueuedGuest[] {
        return [...this.queue];
    }

    /**
     * Update guest status
     */
    updateGuestStatus(guestId: string, status: QueuedGuest['status']): void {
        const guest = this.queue.find(g => g.id === guestId);
        if (!guest) return;

        guest.status = status;
        
        if (status === 'joined' && !guest.joinedAt) {
            guest.joinedAt = new Date();
        }
        if (status === 'on-air' && !guest.onAirAt) {
            guest.onAirAt = new Date();
        }

        // Update positions
        this.updatePositions();
    }

    /**
     * Remove guest from queue
     */
    removeGuest(guestId: string): void {
        this.queue = this.queue.filter(g => g.id !== guestId);
        this.updatePositions();
    }

    /**
     * Move guest up in queue
     */
    moveUp(guestId: string): void {
        const index = this.queue.findIndex(g => g.id === guestId);
        if (index > 0) {
            [this.queue[index], this.queue[index - 1]] = [this.queue[index - 1], this.queue[index]];
            this.updatePositions();
        }
    }

    /**
     * Move guest down in queue
     */
    moveDown(guestId: string): void {
        const index = this.queue.findIndex(g => g.id === guestId);
        if (index < this.queue.length - 1) {
            [this.queue[index], this.queue[index + 1]] = [this.queue[index + 1], this.queue[index]];
            this.updatePositions();
        }
    }

    /**
     * Update queue positions
     */
    private updatePositions(): void {
        this.queue.forEach((guest, index) => {
            guest.position = index + 1;
        });
    }

    /**
     * Clear completed guests
     */
    clearCompleted(): void {
        this.queue = this.queue.filter(g => g.status !== 'completed');
        this.updatePositions();
    }

    /**
     * Get next guest in queue
     */
    getNextGuest(): QueuedGuest | null {
        const next = this.queue.find(g => g.status === 'waiting');
        return next || null;
    }
}

// Singleton instance
let queueInstance: GuestQueueService | null = null;

export function getGuestQueueService(): GuestQueueService {
    if (!queueInstance) {
        queueInstance = new GuestQueueService();
    }
    return queueInstance;
}
